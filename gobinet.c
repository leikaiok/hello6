#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <errno.h>
#include "ril.h"
#include "ril_common.h"
#include "gobinet.h"

#define LOG_TAG "RIL"
#include <utils/Log.h>


static void signal_handle(int signal)
{
    if (signal != SIGINT) {
        return;
    }
    RLOGD("gobinetmonitorquit with signal:%d", signal);
    pthread_exit(NULL);
}

/* ---------------------------------------------------------------------------------------- */
static int get_ipv4_by_str(char *ipaddr, unsigned int *ip)
{
    struct in_addr ipv4;
    if (ipaddr == NULL) {
        return -1;
    }

    if (inet_aton(ipaddr, &ipv4) == 0) {
        return -1;
    }

    *ip = ipv4.s_addr;
    return 0;
}

static unsigned int get_mask_by_ipv4(unsigned int ip)
{
    unsigned int bits = 1;
    unsigned int mask = ~bits;
    unsigned int i_mask;
    int mask_len = 0;

    if (bits & ip) {
      while(bits & ip) {
          bits <<= 1;
          mask <<= 1;
      }
    } else {
      while(!(bits & ip)) {
          bits <<= 1;
          mask <<= 1;
      }
    }

    if ((mask & 0x1) != 0) {
      mask = ntohl(mask);
    }

    i_mask = mask;

    while((mask & 0x1) == 0) {
      mask = (mask >> 1);
      mask_len++;
    }

    mask_len = 32 - mask_len;
    return mask_len;
}

/* ---------------------------------------------------------------------------------------- */
static int write_data_command_req(void *data, int data_len, int if_id)
{
    RLOGD("%s entry", __func__);
    char *node[] = {DATA0, DATA1, DATA2};
    struct stat dummy;
    int cur = 0;
    int written;

    if (!data || data_len <= 0) {
        RLOGE("data is null or data_len error.");
        return -1;
    }

    if (stat(node[if_id], &dummy) != 0) {
        RLOGD("stat path %s fail.", node[if_id]);
        return -1;
    }

    int fd = open(node[if_id], O_RDWR);
    if (fd < 0) {
        RLOGE("open path %s fail.", node[if_id]);
        return -1;
    }

    while(cur < data_len) {
        do {
            written = write(fd, (char *)data+cur, data_len-cur);
        }while(written < 0 && errno == EINTR);

        if (written < 0) {
            close(fd);
            return -1;
        }
        cur += written;
    }
    close(fd);
    RLOGD("write data size:%d", written);
    return 0;
}

static char *read_data_command_rsp(int if_id)
{
    RLOGD("%s entry", __func__);
    char *node[] = {DATA0_CFG, DATA1_CFG, DATA2_CFG};
    struct stat dummy;
    fd_set rd_fd;
    struct timeval tv;
    static char rsp[MAX_RSP_MSG_LENS+1];
    int size;

    if (stat(node[if_id], &dummy) != 0) {
        RLOGD("stat path %s fail.", node[if_id]);
        return NULL;
    }
    int fd = open(node[if_id], O_RDWR);
    if (fd < 0) {
        RLOGE("open path %s fail.", node[if_id]);
        return NULL;
    }

    FD_ZERO(&rd_fd);
    FD_SET(fd, &rd_fd);
    tv.tv_sec = 2;
    tv.tv_usec = 0;

    int ret = select(fd+1, &rd_fd, NULL, NULL, &tv);
    if (ret < 0) {
        RLOGE("select fail.");
        return NULL;
    } else if (ret == 0) {
        RLOGE("select timeout.");
        return NULL;
    } else {
        if (FD_ISSET(fd, &rd_fd)) {
            memset(rsp, 0, sizeof(rsp));
            size = read(fd, rsp, MAX_RSP_MSG_LENS);
            if (size <= 0) {
                RLOGE("read rsp fail.");
                return NULL;
            }
            RLOGD("read_data_response size:%d", size);
        }
    }

    RLOGD("%s exit", __func__);
    return rsp;
}

void ReadResponse(void *param)
{
    RLOGD("%s entry", __func__);
    ResponseParam *pa;
    RIL_Token t;
    char cmd[128] = {0};
    char *temp_ip = NULL;
    char *temp_gw = NULL;
    char *temp_dns = NULL;
    int temp_mtu = 1430;
    char ip[128];
    char gw[128];
    char dns[128];
    unsigned int ipaddr;
    unsigned int mask_len;

    pa = (ResponseParam *)param;
    t = pa->t;
    RLOGD("port:%s,pdn_id:%d,if_id:%d,protocol:%s", pa->port, pa->pdn_id, pa->if_id, pa->protocol);

    mb_data_conn_rsp_msg_s_type *rsp = (mb_data_conn_rsp_msg_s_type *)read_data_command_rsp(pa->if_id);
    if (!rsp) {
        RLOGE("read_data_response NULL.");
        goto error;
    }

    RLOGD("rsp msg type:%d.", rsp->type);
    if (rsp->type == MB_DATACONN_ERR_MSG) {
        RLOGD("msg_type is MB_DATACONN_ERR_MSG.");
    } else if (rsp->type == MB_DATACONN_RSP_MSG) {
        RLOGD("msg_type is MB_DATACONN_RSP_MSG.");
        RLOGD("protocol: %s, v4_status: %d, v6_status: %d", pa->protocol, rsp->data_conn_rsp.v4_pdp_status, rsp->data_conn_rsp.v6_pdp_status);

        if (strcmp(pa->protocol, "IP") == 0) {
            if (rsp->data_conn_rsp.v4_pdp_status != MB_IPV4_CONNECTED) {
                RLOGE("get ipv4 addr error.");
                goto error;
            }
            bzero(ip, sizeof(ip));
            get_ipv4_by_str(rsp->data_conn_rsp.v4_addr.public_ip , &ipaddr);
            mask_len = get_mask_by_ipv4(ipaddr);
            snprintf(ip, sizeof(ip) - 1, "%s/%d", rsp->data_conn_rsp.v4_addr.public_ip, mask_len);
            temp_ip = ip;
            temp_gw = rsp->data_conn_rsp.v4_addr.gateway_ip;
            temp_dns =rsp->data_conn_rsp.v4_addr.primary_dns;
            temp_mtu = rsp->data_conn_rsp.v4_mtu;
            RLOGD("temp_ip:%s, temp_gw:%s, temp_dns:%s, temp_mtu:%d.", temp_ip, temp_gw, temp_dns, temp_mtu);
        } else if (strcmp(pa->protocol, "IPV6") == 0) {
            if (rsp->data_conn_rsp.v6_pdp_status != MB_IPV6_CONNECTED) {
                RLOGE("get ipv6 addr error.");
                goto error;
            }
            temp_ip = rsp->data_conn_rsp.v6_addr.public_ipv6;
            temp_dns =rsp->data_conn_rsp.v6_addr.primary_dnsv6;
            temp_mtu = rsp->data_conn_rsp.v6_mtu;
            RLOGD("temp_ip:%s, temp_dns:%s, temp_mtu:%d.", temp_ip, temp_dns, temp_mtu);
        } else if (strcmp(pa->protocol, "IPV4V6") == 0) {
            if (rsp->data_conn_rsp.v4_pdp_status != MB_IPV4_CONNECTED && rsp->data_conn_rsp.v6_pdp_status != MB_IPV6_CONNECTED) {
                RLOGE("get ipv4 and ipv6 addr error.");
                goto error;
            } else if (rsp->data_conn_rsp.v4_pdp_status == MB_IPV4_CONNECTED && rsp->data_conn_rsp.v6_pdp_status == MB_IPV6_CONNECTED) {
                bzero(ip, sizeof(ip));
                get_ipv4_by_str(rsp->data_conn_rsp.v4_addr.public_ip , &ipaddr);
                mask_len = get_mask_by_ipv4(ipaddr);
                snprintf(ip, sizeof(ip) - 1, "%s/%d %s", rsp->data_conn_rsp.v4_addr.public_ip, mask_len, rsp->data_conn_rsp.v6_addr.public_ipv6);
                temp_ip = ip;

                bzero(gw, sizeof(gw));
                snprintf(gw, sizeof(gw) - 1, "%s", rsp->data_conn_rsp.v4_addr.gateway_ip);
                temp_gw = gw;

                bzero(dns, sizeof(dns));
                snprintf(dns, sizeof(dns) - 1, "%s %s", rsp->data_conn_rsp.v4_addr.primary_dns, rsp->data_conn_rsp.v6_addr.primary_dnsv6);
                temp_dns = dns;

                temp_mtu = rsp->data_conn_rsp.v4_mtu;
                RLOGD("ipv4v6:temp_ip:%s, temp_gw:%s, temp_dns:%s, temp_mtu:%d.", temp_ip, temp_gw, temp_dns, temp_mtu);
            } else if (rsp->data_conn_rsp.v4_pdp_status == MB_IPV4_CONNECTED) {
                temp_ip = rsp->data_conn_rsp.v4_addr.public_ip;
                temp_gw =  rsp->data_conn_rsp.v4_addr.gateway_ip;
                temp_dns =  rsp->data_conn_rsp.v4_addr.primary_dns;
                temp_mtu = rsp->data_conn_rsp.v4_mtu;
                RLOGD("temp_ip:%s, temp_gw:%s, temp_dns:%s, temp_mtu:%d.", temp_ip, temp_gw, temp_dns, temp_mtu);
            } else if (rsp->data_conn_rsp.v6_pdp_status == MB_IPV6_CONNECTED) {
                temp_ip = rsp->data_conn_rsp.v6_addr.public_ipv6;
                temp_dns = rsp->data_conn_rsp.v6_addr.primary_dnsv6;
                temp_mtu = rsp->data_conn_rsp.v6_mtu;
                RLOGD("temp_ip:%s, temp_dns:%s, temp_mtu:%d.", temp_ip, temp_dns, temp_mtu);
            }
        }
    } else {
        RLOGD("msg_type:%d not find.", rsp->type);
    }

    snprintf(cmd, sizeof(cmd), "/system/bin/dhcpcd %s", pa->port);
    system(cmd);
    RLOGD("%s", cmd);

#if RIL_VERSION >= 10
    RIL_Data_Call_Response_v11 DataCallResponse;
#elif RIL_VERSION >= 6
    RIL_Data_Call_Response_v6 DataCallResponse;
#endif

    memset(&DataCallResponse, 0, sizeof(DataCallResponse));
    DataCallResponse.status = PDP_FAIL_NONE;
    DataCallResponse.suggestedRetryTime = -1;
    DataCallResponse.cid = pa->pdn_id;
    DataCallResponse.active = 2;
    DataCallResponse.type = pa->protocol;
    DataCallResponse.ifname = pa->port;
    DataCallResponse.addresses = temp_ip; /* e.g., "192.0.1.3" or "192.0.1.11/16 2001:db8::1/64". */
    DataCallResponse.gateways= temp_gw; /* e.g., "192.0.1.3" or "192.0.1.11 2001:db8::1" */
    DataCallResponse.dnses =temp_dns; /* e.g., "192.0.1.3" or "192.0.1.11 2001:db8::1" */
#if RIL_VERSION >= 10
    DataCallResponse.pcscf = "";
    DataCallResponse.mtu = temp_mtu;
#endif
    if (t == NULL) {
        RLOGD("t is invalid.");
    }

    RIL_onRequestComplete(t, RIL_E_SUCCESS, &DataCallResponse, sizeof(DataCallResponse));
    RLOGD("%s exit", __func__);
    return;

error:
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
}

/* ---------------------------------------------------------------------------------------- */
int gobinet_start(const int pdp_id, const char *pdp_type, const char *apn, const char *user, const char *pass,
                        const char *auth_type, char *port, int if_id)
{
    RLOGD("%s entry, pdp_id:%d, pdp_type:%s, apn:%s, user:%s, pass:%s, auth_type:%s, port:%s, if_id:%d",
           __func__, pdp_id, pdp_type, apn, user, pass, auth_type, port, if_id);
    mb_data_conn_req_msg_s_type data_conn;

    data_conn.msg = MB_DATACONN_REQ_MSG;
    data_conn.data_conn_req.if_id = if_id;
    data_conn.data_conn_req.cid = pdp_id;
    if (strcmp(pdp_type, "IP") == 0) {
        data_conn.data_conn_req.pdp_type = MB_IPV4;
    } else if (strcmp(pdp_type, "IPV6") == 0) {
        data_conn.data_conn_req.pdp_type = MB_IPV6;
    } else {
        data_conn.data_conn_req.pdp_type = MB_IPV4V6;
    }
    strcpy(data_conn.data_conn_req.apn, apn);

    if (write_data_command_req(&data_conn, sizeof(mb_data_conn_req_msg_s_type), if_id) < 0) {
        RLOGE("write_data_command_req ret error.");
        return -1;
    }

    return 0;
}

int gobinet_stop(int pdp_id)
{
    int intf[] = {-1, 0, 1, 2};
    mb_data_disconn_req_msg_s_type disconn_req;
    disconn_req.msg = MB_DATADISCONN_REQ_MSG;
    disconn_req.if_id = pdp_id;

    int err = write_data_command_req(&disconn_req, sizeof(mb_data_disconn_req_msg_s_type), intf[pdp_id]);
    if (err < 0) {
        RLOGE("write_data_command_req,error:%d", err);
        return -1;
    }
    return 0;
}

/* ---------------------------------------------------------------------------------------- */
static int gobinet_msg_to_response(mb_data_conn_rsp_s_type *msg, RIL_Data_Call_Response_v11 *response)
{
    RLOGD("%s entry", __func__);
    if (!msg || !response) {
        return -1;
    }

    char *ip = NULL;
    char *dns = NULL;
    char *gw = NULL;
    char *ifname = NULL;
    char *type = NULL;
    char *pcscf = NULL;
    int mtu = 0;
    int active = 0;
    char *port[] = {"", IF_NAME0, IF_NAME1, IF_NAME2};

    if (msg->v4_pdp_status != 1 && msg->v6_pdp_status != 1) {
        return -1;
    }

    ifname = (char *)malloc(MAX_IF_NAME_LENS+1);
    ip = (char *)malloc(MAX_IP_LEN+1);
    dns = (char *)malloc(MAX_IP_LEN+1);
    gw = (char *)malloc(MAX_IP_LEN+1);
    type = (char *)malloc(MAX_PROTOCOL_LENS+1);
    pcscf = (char *)malloc(MAX_PCSCF_LEN+1);

    if (!ifname || !ip || !dns || !gw || !type || !pcscf) {
        goto error;
    }

    memset(ifname, 0, MAX_IF_NAME_LENS+1);
    memset(ip, 0, MAX_IP_LEN+1);
    memset(dns, 0, MAX_IP_LEN+1);
    memset(gw, 0, MAX_IP_LEN+1);
    memset(type, 0, MAX_PROTOCOL_LENS+1);
    memset(pcscf, 0, MAX_PCSCF_LEN+1);
    memcpy(ifname, port[msg->if_id], strlen(port[msg->if_id]));

    switch(msg->pdp_type) {
        case PDP_TYPE_IPV4:
            memcpy(type, PROTOCOL_IPV4, strlen(PROTOCOL_IPV4));
            if (msg->v4_pdp_status == 1) {
                active = 2;
                memcpy(ip, msg->v4_addr.public_ip, strlen(msg->v4_addr.public_ip));
                memcpy(gw, msg->v4_addr.gateway_ip, strlen(msg->v4_addr.gateway_ip));
                memcpy(dns, msg->v4_addr.primary_dns, strlen(msg->v4_addr.primary_dns));
                mtu = msg->v4_mtu;
            } else {
                active = 0;
            }
            break;

        case PDP_TYPE_IPV6:
            memcpy(type, PROTOCOL_IPV6, strlen(PROTOCOL_IPV6));
            if (msg->v6_pdp_status == 1) {
                active = 2;
                memcpy(ip, msg->v6_addr.public_ipv6, strlen(msg->v6_addr.public_ipv6));
                memcpy(gw, msg->v6_addr.gateway_ipv6, strlen(msg->v6_addr.gateway_ipv6));
                memcpy(dns, msg->v6_addr.primary_dnsv6, strlen(msg->v6_addr.primary_dnsv6));
                mtu = msg->v6_mtu;
            } else {
                active = 0;
            }
            break;

        case PDP_TYPE_IPV4V6:
            memcpy(type, PROTOCOL_IPV4V6, strlen(PROTOCOL_IPV4V6));
            if (msg->v4_pdp_status == 1 && msg->v6_pdp_status == 1) {
                active = 2;
                snprintf(ip, MAX_IP_LEN, "%s %s", msg->v4_addr.public_ip, msg->v6_addr.public_ipv6);
                snprintf(gw, MAX_IP_LEN, "%s %s", msg->v4_addr.gateway_ip, msg->v6_addr.gateway_ipv6);
                snprintf(dns, MAX_IP_LEN, "%s %s", msg->v4_addr.primary_dns, msg->v6_addr.primary_dnsv6);
                mtu = (msg->v4_mtu > msg->v6_mtu) ? msg->v4_mtu : msg->v6_mtu;
            } else if (msg->v4_pdp_status == 1) {
                active = 2;
                memcpy(ip, msg->v4_addr.public_ip, strlen(msg->v4_addr.public_ip));
                memcpy(gw, msg->v4_addr.gateway_ip, strlen(msg->v4_addr.gateway_ip));
                memcpy(dns, msg->v4_addr.primary_dns, strlen(msg->v4_addr.primary_dns));
                mtu = msg->v4_mtu;
            } else if (msg->v6_pdp_status == 1) {
                active = 2;
                memcpy(ip, msg->v6_addr.public_ipv6, strlen(msg->v6_addr.public_ipv6));
                memcpy(gw, msg->v6_addr.gateway_ipv6, strlen(msg->v6_addr.gateway_ipv6));
                memcpy(dns, msg->v6_addr.primary_dnsv6, strlen(msg->v6_addr.primary_dnsv6));
                mtu = msg->v6_mtu;
            } else {
                RLOGE("v4 and v6 status is 0.");
            }
    }

    if (active == 2) {
        response->status = PDP_FAIL_NONE;
    } else {
        response->status = PDP_FAIL_ERROR_UNSPECIFIED;
    }
    response->suggestedRetryTime = -1;
    response->cid = msg->cid;
    response->active = active;
    response->type = type;
    response->ifname = ifname;
    response->addresses = ip;
    response->dnses = dns;
    response->gateways = gw;
    response->pcscf = pcscf;
    response->mtu = mtu;
    return 0;

error:
    if (ifname) free(ifname);
    if (ip) free(ip);
    if (dns) free(dns);
    if (gw) free(gw);
    if (type) free(type);
    if (pcscf) free(pcscf);
    return -1;
}

static int get_connection_state(RIL_Data_Call_Response_v11 *response, int id)
{
    RLOGD("%s entry", __func__);
    char path[MAX_GOBINET_PATH_LEN+1] = {0};
    char buf[MAX_EVENT_LENS+1] = {0};
    mb_data_conn_rsp_msg_s_type rsp;
    mb_data_conn_rsp_s_type *msg = NULL;

    snprintf(path, MAX_GOBINET_PATH_LEN, "%s/data%dcfg", GOBINET_STATUS_ROOT,  id);
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        return -1;
    }

    int size = read(fd, buf, MAX_EVENT_LENS);
    close(fd);

    if (size <= 0)
        return -1;

    int msg_type = *((int *)buf);
    if (msg_type == GOBINET_MSG_TYPE) {
        memset(&rsp, 0, sizeof(mb_data_conn_rsp_msg_s_type));
        memcpy(&rsp, buf, sizeof(mb_data_conn_rsp_msg_s_type));
        msg = &(rsp.data_conn_rsp);
        return gobinet_msg_to_response(msg, response);
    }

    return -1;
}

static int clean_response(RIL_Data_Call_Response_v11 *response)
{
    RLOGD("%s entry", __func__);
    if (!response)
        return -1;

    if (response->type) {
        free(response->type);
        response->type = NULL;
    }

    if (response->ifname) {
        free(response->ifname);
        response->ifname = NULL;
    }

    if (response->addresses) {
        free(response->addresses);
        response->addresses = NULL;
    }

    if (response->dnses) {
        free(response->dnses);
        response->dnses = NULL;
    }

    if (response->gateways) {
        free(response->gateways);
        response->gateways = NULL;
    }

    if (response->pcscf) {
        free(response->pcscf);
        response->pcscf = NULL;
    }
    return 0;
}

static int process_event(char *buf, int len)
{
    RLOGD("%s entry", __func__);
    mb_data_conn_rsp_msg_s_type rsp;
    mb_data_conn_rsp_s_type *msg;
    char cmd[MAX_CMD_LENS+1] = {0};
    char *port[] = {"", IF_NAME0, IF_NAME1, IF_NAME2};
    unsigned char cnt;
    int id = 0;
    RIL_Data_Call_Response_v11 response[MAX_CONN_NUM] = {0};

    if (!buf) {
        return -1;
    }

    memset(&rsp, 0, sizeof(mb_data_conn_rsp_msg_s_type));
    memcpy(&rsp, buf, sizeof(mb_data_conn_rsp_msg_s_type));
    msg = &(rsp.data_conn_rsp);
    if (msg->v4_pdp_status == 1 || msg->v6_pdp_status == 1) {
        return 0;
    }

    snprintf(cmd, MAX_CMD_LENS, "/system/bin/dhcpcd -x %s", port[msg->if_id]);
    system(cmd);

    if (msg->v4_pdp_status == 1) {
        if (0 == gobinet_msg_to_response(msg, &response[id])) {
            id++;
        }
    }

    for (cnt = CONN0; cnt < MAX_CONN_NUM; cnt++) {
        if (cnt == msg->if_id)
            continue;
        if (0 == get_connection_state(&response[id], cnt)) {
            id++;
        }
    }

    if (id <= 0) {
        //RIL_onUnsolicitedResponse(RIL_UNSOL_DATA_CALL_LIST_CHANGED, NULL, 0);
        return 0;
    }

    //RIL_onUnsolicitedResponse(RIL_UNSOL_DATA_CALL_LIST_CHANGED, &response, id*sizeof(RIL_Data_Call_Response_v11));
    for (cnt = 0; cnt < id; cnt++) {
        clean_response(&response[cnt]);
    }
    return 0;
}

/* cycle process datacall status */
static void *gobinet_monitor_thread(void *arg)
{
    RLOGD("%s entry", __func__);
    int fd;
    char buf[MAX_EVENT_LENS+1];
    int size;

    while(1)
    {
        memset(buf, 0, sizeof(buf));
        fd = open(GOBINET_STATUS_PATH, O_RDONLY);
        if (fd < 0) {
            usleep(500000);
            continue;
        }
        size = read(fd, buf, MAX_EVENT_LENS);
        close(fd);

        if (size > 0) {
            process_event(buf, size);
            continue;
        }
        usleep(200000);
    }
}

/* monitor gobinet event report */
int start_gobinet_event_report(void)
{
    pthread_t pid = -1;
    return pthread_create(&pid, NULL, gobinet_monitor_thread, NULL);
}



