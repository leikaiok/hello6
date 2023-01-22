#ifndef GOBINET_H
#define GOBINET_H 1

#include "ril.h"

#define DATA0                   "/sys/gswgobi/data0"
#define DATA0_CFG               "/sys/gswgobi/data0cfg"

#define DATA1                   "/sys/gswgobi/data1"
#define DATA1_CFG               "/sys/gswgobi/data1cfg"

#define DATA2                   "/sys/gswgobi/data2"
#define DATA2_CFG               "/sys/gswgobi/data2cfg"

#define GOBINET_STATUS_PATH     "/sys/gswgobi/gobi_call_status"
#define GOBINET_STATUS_ROOT     "/sys/gswgobi"

#define IF_NAME0                "usb0"
#define IF_NAME1                "usb0.1"
#define IF_NAME2                "usb0.2"
#define MAX_IF_NAME_LENS        16
#define MAX_APN_LENS            64

#define MAX_GOBINET_PATH_LEN    128
#define MAX_EVENT_LENS          100
#define MAX_CMD_LENS            100

#define CONN0                   0
#define CONN1                   1
#define CONN2                   2
#define MAX_CONN_NUM            3

#define GOBINET_MSG_TYPE        0x41

#define IPV4_ADDRSTRLEN         16
#define IPV6_ADDRSTRLEN         46
#define MAX_IP_LEN              40

#define PROTOCOL_IPV4           "IP"
#define PROTOCOL_IPV6           "IPV6"
#define PROTOCOL_IPV4V6         "IPV4V6"

#define PDP_TYPE_IPV4           0
#define PDP_TYPE_IPV6           1
#define PDP_TYPE_IPV4V6         2

#define MAX_PCSCF_LEN           2
#define MAX_PROTOCOL_LENS       16
#define MAX_RSP_MSG_LENS        128

#define MAX_PROFILE_ID          (8)
#define MIN_PROFILE_ID          (1)

#define APN_STRING_MAX          (64+6)
#define MB_INVALID_INTERFACE    (0x09)

typedef enum
{
    MB_IPV4_DISCONNECTED    = 0x00,
    MB_IPV4_CONNECTED       = 0x01,
    MB_IPV4_DISCONNECTING   = 0x02,
    MB_IPV4_CONNECTING      = 0x03,

    MB_IPV6_DISCONNECTED    = MB_IPV4_DISCONNECTED, //= 0x10,
    MB_IPV6_CONNECTED       ,//= 0x11,
    MB_IPV6_DISCONNECTING   ,//= 0x12,
    MB_IPV6_CONNECTING      ,//= 0x13,

    MB_DORMANCY             = 0X20,

    MB_MAX_STATUS         = 0x99
} mb_pdp_state_e_type;



typedef struct
{
    char   public_ip[IPV4_ADDRSTRLEN];
    char   gateway_ip[IPV4_ADDRSTRLEN];
    char   primary_dns[IPV4_ADDRSTRLEN];
    char   secondary_dns[IPV4_ADDRSTRLEN];
} v4_conf_s_type;

typedef struct
{
    char  public_ipv6[IPV6_ADDRSTRLEN];
    char  gateway_ipv6[IPV6_ADDRSTRLEN];
    char  primary_dnsv6[IPV6_ADDRSTRLEN];
    char  secondary_dnsv6[IPV6_ADDRSTRLEN];
} v6_conf_s_type;


typedef enum {
    MB_IPV4    = 0,
    MB_IPV6    = 1,
    MB_IPV4V6  = 2,
}pdp_type_e;


typedef enum {
    MB_DATACONN_REQ_MSG     = 0x00,
    MB_DATACONN_RSP_MSG     = 0x01,
    MB_DATACONN_ERR_MSG     = 0x02,
    MB_DATACONN_WAIT_MSG    = 0x03,

    MB_DATADISCONN_REQ_MSG  = 0x10,
    MB_DATADISCONN_RSP_MSG  = 0x11,
    MB_DATADISCONN_WAIT_MSG = 0x11,
}mb_msg_e_type;


typedef enum
{
    MB_FIRST_INTERFACE = 0x00,
    MB_SEC_INTERFACE   = 0x01,
    MB_THIRD_INTERFACE = 0X02,
    MB_MAX_INTERFACE   = MB_INVALID_INTERFACE
} mb_interface_id_e_type;


/*------------------- connect request -------------------*/
typedef struct
{
    mb_interface_id_e_type  if_id;
    int  cid;
    int  pdp_type;
    char apn[APN_STRING_MAX];
} mb_data_conn_req_s_type;   //其中int pdp_type, char apn[64] 为空。

typedef struct {
    int msg;
    mb_data_conn_req_s_type data_conn_req;
}mb_data_conn_req_msg_s_type;


/*------------------- connect response -------------------*/
typedef struct
{
    mb_interface_id_e_type if_id;
    int cid;
    int pdp_type;

    int v4_pdp_status;
    v4_conf_s_type   v4_addr;
    int v4_mtu;

    int v6_pdp_status;
    v6_conf_s_type v6_addr;
    int v6_mtu;

} mb_data_conn_rsp_s_type;

typedef struct {
    int type;
    mb_data_conn_rsp_s_type data_conn_rsp;
}mb_data_conn_rsp_msg_s_type;


/*------------------- disconnect response -------------------*/


/*------------------- disconnect response -------------------*/
typedef struct
{
    int msg;
    mb_interface_id_e_type  if_id;
} mb_data_disconn_req_msg_s_type;


typedef struct {
    char port[10];
    int pdn_id;
    int if_id;
    char protocol[10];
    RIL_Token t;
}ResponseParam;

/*------------------- Function declaration  -------------------*/
void ReadResponse(void *param);
int gobinet_start(const int pdp_id, const char *pdp_type, const char *apn, const char *user, const char *pass,
                        const char *auth_type, char *port, int if_id);
int gobinet_stop(int pdp_id);
int start_gobinet_event_report(void);


#endif /*GOBINET_H */



