/* //device/system/reference-ril/sms.h
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
#ifndef SMS_H
#define SMS_H

typedef enum
{
    SMS_GENERAL     = 0,
    SMS_SEND_REPORT = 1,
    SMS_BROADCAST   = 2,
    SMS_NONE        = 10
} SMS_TYPE;

/* New Sms Notification */
void onNewSmsNotification(const char *s);

/* New Sms Read and report */
void onNewSmsArrived(const char *sms_pdu);

/* Deleted SMS by index */
int deleteSmsByIndex(int index);

/* unsolicited unread SMS */
void unsolicitedUnreadSms(void *param __unused);

#endif