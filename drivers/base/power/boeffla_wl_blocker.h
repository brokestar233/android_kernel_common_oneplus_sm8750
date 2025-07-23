/*
 * Author: andip71, 01.09.2017
 *
 * Version 1.1.0
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#define BOEFFLA_WL_BLOCKER_VERSION	"1.1.0"

#define LIST_WL_DEFAULT				"gauge;sscrpcd:2310;b0000000.qcom,cnss-peach;wired wakelock;cne_wl_;radio-data-interface;fastrpc-secure;eventpoll;event0;event8;event2;event3;9-0028;amd_lock;lux_aod_lock;a600000.ssusb;qcom_rx_wakelock;wlan;wlan_wow_wl;NETLINK;IPA_WS;[timerfd];wlan_ipa;wlan_pno_wl;DIAG_WS;qcom_sap_wakelock;pmo_wow_wl;898000.qcom,qup_uart;rmnet_ctl;hal_bluetooth_lock;SensorsHAL_WAKEUP;ena600000.ssusb;gesture_prox_lock;gnss_hal;prox_lock;IPA_CLIENT_APPS_LAN_CONS;IPA_CLIENT_APPS_WAN_LOW_LAT_CONS;RMNET_SHS;IPA_CLIENT_APPS_WAN_COAL_CONS;oplus_shaking_lock;8-0028;phone_prox_lock;qrtr_ws;rmnet_ipa%d;wlan_ap_assoc_lost_wl;radio-data-interface;tftp_server_wakelock;pedometer_minute_lock;wlan_roam_ho_wl;10-0028;vdev_start;event8;eventpoll"

#define LENGTH_LIST_WL				8192
#define LENGTH_LIST_WL_DEFAULT		8192
#define LENGTH_LIST_WL_SEARCH		LENGTH_LIST_WL + LENGTH_LIST_WL_DEFAULT + 5
