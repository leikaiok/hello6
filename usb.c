#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define LOG_TAG "RIL"
#include <utils/Log.h>

#define ARRAY_SIZE(a) (sizeof(a)/sizeof(a[0]))
#define USBID_LEN 4
#define MAX_PATH 256

struct module_info_s {
    const char idVendor[USBID_LEN+1];
    const char idProduct[USBID_LEN+1];
    char dm_inf;
    char at_inf;
    char ppp_inf;
    char gps_inf;
    char ndis_inf;
    const char *driver;
};

struct usb_device_info_s {
    const struct module_info_s *module_info;
    char usbdevice_pah[MAX_PATH];
    char ttyDM[16];
    char ttyAT[16];
    char ttyPPP[16];
    char ttyGPS[16];
    char ttyNDIS[16];
};

static struct usb_device_info_s s_usb_device_info;

static const struct module_info_s module_info_table[] = {
    {"305A", "1418", 0, 1, 2, 3, 4, "option"}, //GM551A
};

static const struct module_info_s * get_module_info(char idVendor[USBID_LEN+1], char idProduct[USBID_LEN+1]) {
    size_t i;
    for (i = 0; i < ARRAY_SIZE(module_info_table); i++) {
        if (!strncasecmp(module_info_table[i].idVendor, idVendor, USBID_LEN) && !strncasecmp(module_info_table[i].idProduct, idProduct, USBID_LEN))
            return &module_info_table[i];
    }
    return NULL;
}

int get_ttyAT(char *out_ttyname) {
    if (!s_usb_device_info.ttyAT[0]) {
        RLOGE("cannot find AT Port");
        return -1;
    }
    strcpy(out_ttyname, s_usb_device_info.ttyAT);
    return 0;
}

int get_ndisname(char *out_ttyname) {
    if (!s_usb_device_info.ttyNDIS[0]) {
        RLOGE("cannot find NDIS Port");
        return -1;
    }
    strcpy(out_ttyname, s_usb_device_info.ttyNDIS);
    return 0;
}

static int find_module(struct usb_device_info_s *pusb_device_info)
{
    DIR *pDir;
    int fd;
    char filename[MAX_PATH];
    int find_usb_device = 0;
    struct stat statbuf;
    struct dirent* ent = NULL;
    struct usb_device_info_s usb_device_info;
    const char *dir = "/sys/bus/usb/devices";
    
    if ((pDir = opendir(dir)) == NULL)  {
        RLOGE("Cannot open directory:%s/", dir);
        return 0;
    }

    while ((ent = readdir(pDir)) != NULL)  {
        memset(&usb_device_info, 0, sizeof(usb_device_info));
        sprintf(filename, "%s/%s", dir, ent->d_name);

        lstat(filename, &statbuf);
        if (S_ISLNK(statbuf.st_mode))  {
            char idVendor[USBID_LEN+1] = {0};
            char idProduct[USBID_LEN+1] = {0};

            sprintf(filename, "%s/%s/idVendor", dir, ent->d_name);
            fd = open(filename, O_RDONLY);
            if (fd > 0) {
                read(fd, idVendor, USBID_LEN);
                close(fd);
            }

            sprintf(filename, "%s/%s/idProduct", dir, ent->d_name);
            fd = open(filename, O_RDONLY);
            if (fd > 0) {
                read(fd, idProduct, USBID_LEN);
                close(fd);
            }

            if (!(usb_device_info.module_info = get_module_info(idVendor, idProduct)))
                continue;

            snprintf(usb_device_info.usbdevice_pah, sizeof(usb_device_info.usbdevice_pah), "%s/%s", dir, ent->d_name);
            memcpy(pusb_device_info, &usb_device_info, sizeof(struct usb_device_info_s));
            find_usb_device++;
            RLOGD("find gosuncn module %s idVendor=%s idProduct=%s", usb_device_info.usbdevice_pah, idVendor, idProduct);
            break;
        }
    }

    closedir(pDir);
    return find_usb_device;
}

static int find_ttyname(int usb_interface, const char *usbdevice_pah, char *out_ttyname)
{
    DIR *pDir;
    struct dirent* ent = NULL;
    char dir[MAX_PATH]={0};

    if(usb_interface < 0) {
        return -1;
    }

    snprintf(dir, sizeof(dir), "%s:1.%d", usbdevice_pah, usb_interface);
    if ((pDir = opendir(dir)) == NULL) {
        RLOGE("Cannot open directory:%s/", dir);
        return -1;
    }

    while ((ent = readdir(pDir)) != NULL) {
        if (strncmp(ent->d_name, "tty", 3) == 0) {
            RLOGD("find %s/%s", dir, ent->d_name);
            strcpy(out_ttyname, ent->d_name);
            break;
        }
    }
    closedir(pDir);

    if (strcmp(out_ttyname, "tty") == 0) { //find tty not ttyUSBx or ttyACMx
        strcat(dir, "/tty");
        if ((pDir = opendir(dir)) == NULL)  {
            RLOGE("Cannot open directory:%s", dir);
            return -1;
        }

        while ((ent = readdir(pDir)) != NULL)  {
            if (strncmp(ent->d_name, "tty", 3) == 0) {
                RLOGD("find %s/%s", dir, ent->d_name);
                strcpy(out_ttyname, ent->d_name);
                break;
            }
        }
        closedir(pDir);
    }

    return 0;
}


int detect_module(void)
{
    struct usb_device_info_s usb_device_info = {0};

    if (find_module(&usb_device_info) == 0) {
        return -1;
    }

    sleep(1); //wait usb driver load

    if (find_ttyname(usb_device_info.module_info->dm_inf, usb_device_info.usbdevice_pah, usb_device_info.ttyDM) == 0) {
        RLOGD("ttyDM = %s", usb_device_info.ttyDM);
    }
    if (find_ttyname(usb_device_info.module_info->at_inf, usb_device_info.usbdevice_pah, usb_device_info.ttyAT) == 0) {
        RLOGD("ttyAT = %s", usb_device_info.ttyAT);
    }
    if (find_ttyname(usb_device_info.module_info->ppp_inf, usb_device_info.usbdevice_pah, usb_device_info.ttyPPP) == 0) {
        RLOGD("ttyPPP = %s", usb_device_info.ttyPPP);
    }
    if (find_ttyname(usb_device_info.module_info->gps_inf, usb_device_info.usbdevice_pah, usb_device_info.ttyGPS) == 0) {
        RLOGD("ttyGPS = %s", usb_device_info.ttyGPS);
    }
    if (find_ttyname(usb_device_info.module_info->ndis_inf, usb_device_info.usbdevice_pah, usb_device_info.ttyNDIS) == 0) {
        RLOGD("ttyNDIS = %s", usb_device_info.ttyNDIS);
    }

    memcpy(&s_usb_device_info, &usb_device_info, sizeof(usb_device_info));

    return 0;
}



