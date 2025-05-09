// SPDX-License-Identifier: GPL-2.0-only
/*
################################################################################
#
# r8126 is the Linux device driver released for Realtek 5 Gigabit Ethernet
# controllers with PCI-Express interface.
#
# Copyright(c) 2024 Realtek Semiconductor Corp. All rights reserved.
#
# This program is free software; you can redistribute it and/or modify it
# under the terms of the GNU General Public License as published by the Free
# Software Foundation; either version 2 of the License, or (at your option)
# any later version.
#
# This program is distributed in the hope that it will be useful, but WITHOUT
# ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
# FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
# more details.
#
# You should have received a copy of the GNU General Public License along with
# this program; if not, see <http://www.gnu.org/licenses/>.
#
# Author:
# Realtek NIC software team <nicfae@realtek.com>
# No. 2, Innovation Road II, Hsinchu Science Park, Hsinchu 300, Taiwan
#
################################################################################
*/

/************************************************************************************
 *  This product is covered by one or more of the following patents:
 *  US6,570,884, US6,115,776, and US6,327,625.
 ***********************************************************************************/

#include <linux/module.h>
#include <linux/version.h>
#include <linux/pci.h>
#include <linux/netdevice.h>
#include <linux/delay.h>
#include <linux/mii.h>
#include <linux/in.h>
#include <linux/ethtool.h>
#include <linux/rtnetlink.h>

#include "r8126.h"
#include "r8126_ptp.h"

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5,6,0)
static inline struct timespec timespec64_to_timespec(const struct timespec64 ts64)
{
        return *(const struct timespec *)&ts64;
}

static inline struct timespec64 timespec_to_timespec64(const struct timespec ts)
{
        return *(const struct timespec64 *)&ts;
}
#endif

static int _rtl8126_phc_gettime(struct rtl8126_private *tp, struct timespec64 *ts64)
{
        //get local time
        RTL_W16(tp, PTP_TIME_CORRECT_CMD_8125, (PTP_CMD_LATCHED_LOCAL_TIME | PTP_EXEC_CMD));

        /* nanoseconds */
        //0x6808[29:0]
        ts64->tv_nsec = (RTL_R32(tp, PTP_SOFT_CONFIG_Time_NS_8125) & 0x3fffffff);

        /* seconds */
        //0x680C[47:0]
        ts64->tv_sec = RTL_R16(tp, PTP_SOFT_CONFIG_Time_S_8125 + 4);
        ts64->tv_sec <<= 32;
        ts64->tv_sec |= RTL_R32(tp, PTP_SOFT_CONFIG_Time_S_8125);

        return 0;
}

static int _rtl8126_phc_settime(struct rtl8126_private *tp, const struct timespec64 *ts64)
{
        /* nanoseconds */
        //0x6808[29:0]
        RTL_W32(tp, PTP_SOFT_CONFIG_Time_NS_8125, (ts64->tv_nsec & 0x3fffffff));

        /* seconds */
        //0x680C[47:0]
        RTL_W32(tp, PTP_SOFT_CONFIG_Time_S_8125, ts64->tv_sec);
        RTL_W16(tp, PTP_SOFT_CONFIG_Time_S_8125 + 4, (ts64->tv_sec >> 32));

        //set local time
        RTL_W16(tp, PTP_TIME_CORRECT_CMD_8125, (PTP_CMD_SET_LOCAL_TIME | PTP_EXEC_CMD));

        return 0;
}

static int _rtl8126_phc_adjtime(struct rtl8126_private *tp, s64 delta)
{
        struct timespec64 d;
        bool negative = false;
        u64 tohw;
        u32 nsec;
        u64 sec;

        if (delta < 0) {
                negative = true;
                tohw = -delta;
        } else {
                tohw = delta;
        }

        d = ns_to_timespec64(tohw);

        nsec = d.tv_nsec;
        sec = d.tv_sec;

        if (negative) {
                nsec = -nsec;
                sec = -sec;
        }

        nsec &= 0x3fffffff;
        sec &= 0x0000ffffffffffff;

        if (negative) {
                nsec |= PTP_SOFT_CONFIG_TIME_NS_NEGATIVE;
                sec |= PTP_SOFT_CONFIG_TIME_S_NEGATIVE;
        }

        /* nanoseconds */
        //0x6808[29:0]
        RTL_W32(tp, PTP_SOFT_CONFIG_Time_NS_8125, nsec);

        /* seconds */
        //0x680C[47:0]
        RTL_W32(tp, PTP_SOFT_CONFIG_Time_S_8125, sec);
        RTL_W16(tp, PTP_SOFT_CONFIG_Time_S_8125 + 4, (sec >> 32));

        //adjust local time
        //RTL_W16(tp, PTP_TIME_CORRECT_CMD_8125, (PTP_CMD_DRIFT_LOCAL_TIME | PTP_EXEC_CMD));
        RTL_W16(tp, PTP_TIME_CORRECT_CMD_8125, (PTP_CMD_SET_LOCAL_TIME | PTP_EXEC_CMD));

        return 0;
}

static int rtl8126_phc_adjtime(struct ptp_clock_info *ptp, s64 delta)
{
        struct rtl8126_private *tp = container_of(ptp, struct rtl8126_private, ptp_clock_info);
        int ret;

        //netif_info(tp, drv, tp->dev, "phc adjust time\n");

        rtnl_lock();
        ret = _rtl8126_phc_adjtime(tp, delta);
        rtnl_unlock();

        return ret;
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(6,2,0)
/*
1ppm means every 125MHz plus 125Hz. It also means every 8ns minus 8ns*10^(-6)

1ns=2^30 sub_ns

8ns*10^(-6) = 8 * 2^30 sub_ns * 10^(-6) = 2^33 sub_ns * 10^(-6) = 8590 = 0x218E sub_ns

1ppb means every 125MHz plus 0.125Hz. It also means every 8ns minus 8ns*10^(-9)

1ns=2^30 sub_ns

8ns*10^(-9) = 8 * 2^30 sub_ns * 10^(-9) = 2^33 sub_ns * 10^(-9) = 8.59 sub_ns = 9 sub_ns
*/
static int _rtl8126_phc_adjfreq(struct ptp_clock_info *ptp, s32 ppb)
{
        struct rtl8126_private *tp = container_of(ptp, struct rtl8126_private, ptp_clock_info);
        bool negative = false;
        u32 sub_ns;

        if (ppb < 0) {
                negative = true;
                ppb = -ppb;
        }

        sub_ns = ppb * 9;
        if (negative) {
                sub_ns = -sub_ns;
                sub_ns &= 0x3fffffff;
                sub_ns |= PTP_ADJUST_TIME_NS_NEGATIVE;
        } else
                sub_ns &= 0x3fffffff;

        /* nanoseconds */
        //0x6808[29:0]
        RTL_W32(tp, PTP_SOFT_CONFIG_Time_NS_8125, sub_ns);

        //adjust local time
        RTL_W16(tp, PTP_TIME_CORRECT_CMD_8125, (PTP_CMD_DRIFT_LOCAL_TIME | PTP_EXEC_CMD));
        //RTL_W16(tp, PTP_TIME_CORRECT_CMD_8125, (PTP_CMD_SET_LOCAL_TIME | PTP_EXEC_CMD));

        return 0;
}

static int rtl8126_phc_adjfreq(struct ptp_clock_info *ptp, s32 delta)
{
        //struct rtl8126_private *tp = container_of(ptp, struct rtl8126_private, ptp_clock_info);

        //netif_info(tp, drv, tp->dev, "phc adjust freq\n");

        if (delta > ptp->max_adj || delta < -ptp->max_adj)
                return -EINVAL;

        _rtl8126_phc_adjfreq(ptp, delta);

        return 0;
}
#endif //LINUX_VERSION_CODE < KERNEL_VERSION(6,2,0)

static int rtl8126_phc_gettime(struct ptp_clock_info *ptp, struct timespec64 *ts64)
{
        struct rtl8126_private *tp = container_of(ptp, struct rtl8126_private, ptp_clock_info);
        int ret;

        //netif_info(tp, drv, tp->dev, "phc get ts\n");

        rtnl_lock();
        ret = _rtl8126_phc_gettime(tp, ts64);
        rtnl_unlock();

        return ret;
}

static int rtl8126_phc_settime(struct ptp_clock_info *ptp,
                               const struct timespec64 *ts64)
{
        struct rtl8126_private *tp = container_of(ptp, struct rtl8126_private, ptp_clock_info);
        int ret;

        //netif_info(tp, drv, tp->dev, "phc set ts\n");

        rtnl_lock();
        ret = _rtl8126_phc_settime(tp, ts64);
        rtnl_unlock();

        return ret;
}

static int rtl8126_phc_enable(struct ptp_clock_info *ptp,
                              struct ptp_clock_request *rq, int on)
{
        struct rtl8126_private *tp = container_of(ptp, struct rtl8126_private, ptp_clock_info);
        u16 ptp_ctrl;

        //netif_info(tp, drv, tp->dev, "phc enable type %x on %d\n", rq->type, on);

        switch (rq->type) {
        case PTP_CLK_REQ_PPS:
                rtnl_lock();
                ptp_ctrl = RTL_R16(tp, PTP_CTRL_8125);
                ptp_ctrl &= ~BIT_15;
                if (on)
                        ptp_ctrl |= BIT_14;
                else
                        ptp_ctrl &= ~BIT_14;
                RTL_W16(tp, PTP_CTRL_8125, ptp_ctrl);
                rtnl_unlock();
                return 0;
        default:
                return -EOPNOTSUPP;
        }
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(6,11,0)
int rtl8126_get_ts_info(struct net_device *netdev,
                        struct ethtool_ts_info *info)
#else
int rtl8126_get_ts_info(struct net_device *netdev,
                        struct kernel_ethtool_ts_info *info)
#endif
{
        struct rtl8126_private *tp = netdev_priv(netdev);

        /* we always support timestamping disabled */
        info->rx_filters = BIT(HWTSTAMP_FILTER_NONE);

        if (tp->HwSuppPtpVer == 0)
                return ethtool_op_get_ts_info(netdev, info);

        info->so_timestamping =  SOF_TIMESTAMPING_TX_SOFTWARE |
                                 SOF_TIMESTAMPING_RX_SOFTWARE |
                                 SOF_TIMESTAMPING_SOFTWARE |
                                 SOF_TIMESTAMPING_TX_HARDWARE |
                                 SOF_TIMESTAMPING_RX_HARDWARE |
                                 SOF_TIMESTAMPING_RAW_HARDWARE;

        if (tp->ptp_clock)
                info->phc_index = ptp_clock_index(tp->ptp_clock);
        else
                info->phc_index = -1;

        info->tx_types = BIT(HWTSTAMP_TX_OFF) | BIT(HWTSTAMP_TX_ON);

        info->rx_filters = BIT(HWTSTAMP_FILTER_NONE) |
                           BIT(HWTSTAMP_FILTER_PTP_V2_EVENT) |
                           BIT(HWTSTAMP_FILTER_PTP_V2_L4_EVENT) |
                           BIT(HWTSTAMP_FILTER_PTP_V2_SYNC) |
                           BIT(HWTSTAMP_FILTER_PTP_V2_L4_SYNC) |
                           BIT(HWTSTAMP_FILTER_PTP_V2_DELAY_REQ) |
                           BIT(HWTSTAMP_FILTER_PTP_V2_L4_DELAY_REQ);

        return 0;
}

static const struct ptp_clock_info rtl_ptp_clock_info = {
        .owner      = THIS_MODULE,
        .n_alarm    = 0,
        .n_ext_ts   = 0,
        .n_per_out  = 0,
        .n_pins     = 0,
        .pps        = 1,
#if LINUX_VERSION_CODE < KERNEL_VERSION(6,2,0)
        .adjfreq    = rtl8126_phc_adjfreq,
#endif //LINUX_VERSION_CODE < KERNEL_VERSION(6,2,0)
        .adjtime    = rtl8126_phc_adjtime,
        .gettime64  = rtl8126_phc_gettime,
        .settime64  = rtl8126_phc_settime,
        .enable     = rtl8126_phc_enable,
};

static int rtl8126_ptp_egresstime(struct rtl8126_private *tp, struct timespec64 *ts64, u32 regnum)
{
        /* nanoseconds */
        //[29:0]
        ts64->tv_nsec = rtl8126_mac_ocp_read(tp, PTP_EGRESS_TIME_BASE_NS_8125 + regnum * 16 + 2);
        ts64->tv_nsec <<= 16;
        ts64->tv_nsec |= rtl8126_mac_ocp_read(tp, PTP_EGRESS_TIME_BASE_NS_8125 + regnum * 16);
        ts64->tv_nsec &= 0x3fffffff;

        /* seconds */
        //[47:0]
        ts64->tv_sec = rtl8126_mac_ocp_read(tp, PTP_EGRESS_TIME_BASE_S_8125 + regnum * 16 + 4);
        ts64->tv_sec <<= 16;
        ts64->tv_sec |= rtl8126_mac_ocp_read(tp, PTP_EGRESS_TIME_BASE_S_8125 + regnum * 16 + 2);
        ts64->tv_sec <<= 16;
        ts64->tv_sec |= rtl8126_mac_ocp_read(tp, PTP_EGRESS_TIME_BASE_S_8125 + regnum * 16);
        ts64->tv_sec &= 0x0000ffffffffffff;

        return 0;
}

static void rtl8126_ptp_tx_hwtstamp(struct rtl8126_private *tp)
{
        struct sk_buff *skb = tp->ptp_tx_skb;
        struct skb_shared_hwtstamps shhwtstamps = {0};
        struct timespec64 ts64;
        u32 regnum;

        RTL_W8(tp, PTP_ISR_8125, PTP_ISR_TOK | PTP_ISR_TER);

        //IO 0x2302 bit 10~11 WR_PTR
        regnum = RTL_R16(tp, 0x2032) & 0x0C00;
        regnum >>= 10;
        regnum = (regnum + 3) % 4;

        rtnl_lock();
        rtl8126_ptp_egresstime(tp, &ts64, regnum);
        rtnl_unlock();

        /* Upper 32 bits contain s, lower 32 bits contain ns. */
        shhwtstamps.hwtstamp = ktime_set(ts64.tv_sec,
                                         ts64.tv_nsec);

        /* Clear the lock early before calling skb_tstamp_tx so that
         * applications are not woken up before the lock bit is clear. We use
         * a copy of the skb pointer to ensure other threads can't change it
         * while we're notifying the stack.
         */
        tp->ptp_tx_skb = NULL;
        clear_bit_unlock(__RTL8126_PTP_TX_IN_PROGRESS, &tp->state);

        /* Notify the stack and free the skb after we've unlocked */
        skb_tstamp_tx(skb, &shhwtstamps);
        dev_kfree_skb_any(skb);
}

#define RTL8126_PTP_TX_TIMEOUT      (HZ * 15)
static void rtl8126_ptp_tx_work(struct work_struct *work)
{
        struct rtl8126_private *tp = container_of(work, struct rtl8126_private,
                                     ptp_tx_work);

        if (!tp->ptp_tx_skb)
                return;

        if (time_is_before_jiffies(tp->ptp_tx_start +
                                   RTL8126_PTP_TX_TIMEOUT)) {
                dev_kfree_skb_any(tp->ptp_tx_skb);
                tp->ptp_tx_skb = NULL;
                clear_bit_unlock(__RTL8126_PTP_TX_IN_PROGRESS, &tp->state);
                tp->tx_hwtstamp_timeouts++;
                /* Clear the tx valid bit in TSYNCTXCTL register to enable
                 * interrupt
                 */
                RTL_W8(tp, PTP_ISR_8125, PTP_ISR_TOK | PTP_ISR_TER);
                return;
        }

        if (RTL_R8(tp, PTP_ISR_8125) & (PTP_ISR_TOK))
                rtl8126_ptp_tx_hwtstamp(tp);
        else
                /* reschedule to check later */
                schedule_work(&tp->ptp_tx_work);

}

static int rtl8126_hwtstamp_enable(struct rtl8126_private *tp, bool enable)
{
        RTL_W16(tp, PTP_CTRL_8125, 0);
        if (enable) {
                u16 ptp_ctrl;
                struct timespec64 ts64;

                //clear ptp isr
                RTL_W8(tp, PTP_ISR_8125, 0xff);
                //ptp source 0:gphy 1:mac
                rtl8126_mac_ocp_write(tp, 0xDC00, rtl8126_mac_ocp_read(tp, 0xDC00) | BIT_6);
                //enable ptp
                ptp_ctrl = (BIT_0 | BIT_3 | BIT_4 | BIT_6 | BIT_10 | BIT_12);
                if (tp->ptp_master_mode)
                        ptp_ctrl |= BIT_1;
                RTL_W16(tp, PTP_CTRL_8125, ptp_ctrl);

                //set system time
                /*
                if (ktime_to_timespec64_cond(ktime_get_real(), &ts64))
                _rtl8126_phc_settime(tp, timespec64_to_timespec(ts64));
                */
                ktime_get_real_ts64(&ts64);
                _rtl8126_phc_settime(tp, &ts64);
        }

        return 0;
}

static long rtl8126_ptp_create_clock(struct rtl8126_private *tp)
{
        struct net_device *netdev = tp->dev;
        long err;

        if (!IS_ERR_OR_NULL(tp->ptp_clock))
                return 0;

        if (tp->HwSuppPtpVer == 0) {
                tp->ptp_clock = NULL;
                return -EOPNOTSUPP;
        }

        tp->ptp_clock_info = rtl_ptp_clock_info;
        snprintf(tp->ptp_clock_info.name, sizeof(tp->ptp_clock_info.name),
                 "%pm", tp->dev->dev_addr);
        tp->ptp_clock_info.max_adj = 119304647;
        tp->ptp_clock = ptp_clock_register(&tp->ptp_clock_info, &tp->pci_dev->dev);
        if (IS_ERR(tp->ptp_clock)) {
                err = PTR_ERR(tp->ptp_clock);
                tp->ptp_clock = NULL;
                netif_err(tp, drv, tp->dev, "ptp_clock_register failed\n");
                return err;
        } else
                netif_info(tp, drv, tp->dev, "registered PHC device on %s\n", netdev->name);

        return 0;
}

void rtl8126_ptp_reset(struct rtl8126_private *tp)
{
        if (!tp->ptp_clock)
                return;

        netif_info(tp, drv, tp->dev, "reset PHC clock\n");

        rtl8126_hwtstamp_enable(tp, false);
}

void rtl8126_ptp_init(struct rtl8126_private *tp)
{
        /* obtain a PTP device, or re-use an existing device */
        if (rtl8126_ptp_create_clock(tp))
                return;

        /* we have a clock so we can initialize work now */
        INIT_WORK(&tp->ptp_tx_work, rtl8126_ptp_tx_work);

        /* reset the PTP related hardware bits */
        rtl8126_ptp_reset(tp);

        return;
}

void rtl8126_ptp_suspend(struct rtl8126_private *tp)
{
        if (!tp->ptp_clock)
                return;

        netif_info(tp, drv, tp->dev, "suspend PHC clock\n");

        rtl8126_hwtstamp_enable(tp, false);

        /* ensure that we cancel any pending PTP Tx work item in progress */
        cancel_work_sync(&tp->ptp_tx_work);
}

void rtl8126_ptp_stop(struct rtl8126_private *tp)
{
        struct net_device *netdev = tp->dev;

        netif_info(tp, drv, tp->dev, "stop PHC clock\n");

        /* first, suspend PTP activity */
        rtl8126_ptp_suspend(tp);

        /* disable the PTP clock device */
        if (tp->ptp_clock) {
                ptp_clock_unregister(tp->ptp_clock);
                tp->ptp_clock = NULL;
                netif_info(tp, drv, tp->dev, "removed PHC on %s\n",
                           netdev->name);
        }
}

static int rtl8126_set_tstamp(struct net_device *netdev, struct ifreq *ifr)
{
        struct rtl8126_private *tp = netdev_priv(netdev);
        struct hwtstamp_config config;
        bool hwtstamp = 0;

        //netif_info(tp, drv, tp->dev, "ptp set ts\n");

        if (copy_from_user(&config, ifr->ifr_data, sizeof(config)))
                return -EFAULT;

        if (config.flags)
                return -EINVAL;

        switch (config.tx_type) {
        case HWTSTAMP_TX_ON:
                hwtstamp = 1;
        case HWTSTAMP_TX_OFF:
                break;
        case HWTSTAMP_TX_ONESTEP_SYNC:
        default:
                return -ERANGE;
        }

        switch (config.rx_filter) {
        case HWTSTAMP_FILTER_PTP_V2_EVENT:
        case HWTSTAMP_FILTER_PTP_V2_L2_EVENT:
        case HWTSTAMP_FILTER_PTP_V2_L4_EVENT:
        case HWTSTAMP_FILTER_PTP_V2_SYNC:
        case HWTSTAMP_FILTER_PTP_V2_L2_SYNC:
        case HWTSTAMP_FILTER_PTP_V2_L4_SYNC:
        case HWTSTAMP_FILTER_PTP_V2_DELAY_REQ:
        case HWTSTAMP_FILTER_PTP_V2_L2_DELAY_REQ:
        case HWTSTAMP_FILTER_PTP_V2_L4_DELAY_REQ:
                config.rx_filter = HWTSTAMP_FILTER_PTP_V2_EVENT;
                hwtstamp = 1;
        case HWTSTAMP_FILTER_NONE:
                break;
        default:
                return -ERANGE;
        }

        if (tp->hwtstamp_config.tx_type != config.tx_type ||
            tp->hwtstamp_config.rx_filter != config.rx_filter) {
                tp->hwtstamp_config = config;
                rtl8126_hwtstamp_enable(tp, hwtstamp);
        }

        return copy_to_user(ifr->ifr_data, &config,
                            sizeof(config)) ? -EFAULT : 0;
}

static int rtl8126_get_tstamp(struct net_device *netdev, struct ifreq *ifr)
{
        struct rtl8126_private *tp = netdev_priv(netdev);

        //netif_info(tp, drv, tp->dev, "ptp get ts\n");

        return copy_to_user(ifr->ifr_data, &tp->hwtstamp_config,
                            sizeof(tp->hwtstamp_config)) ? -EFAULT : 0;
}

int rtl8126_ptp_ioctl(struct net_device *netdev, struct ifreq *ifr, int cmd)
{
        int ret;

        //netif_info(tp, drv, tp->dev, "ptp ioctl\n");

        switch (cmd) {
#ifdef ENABLE_PTP_SUPPORT
        case SIOCSHWTSTAMP:
                ret = rtl8126_set_tstamp(netdev, ifr);
                break;
        case SIOCGHWTSTAMP:
                ret = rtl8126_get_tstamp(netdev, ifr);
                break;
#endif
        default:
                ret = -EOPNOTSUPP;
                break;
        }

        return ret;
}

void rtl8126_rx_ptp_pktstamp(struct rtl8126_private *tp, struct sk_buff *skb,
                             struct RxDescV3 *descv3)
{
        time64_t tv_sec;
        long tv_nsec;

        tv_sec = le32_to_cpu(descv3->RxDescTimeStamp.TimeStampHigh) +
                 ((u64)le32_to_cpu(descv3->RxDescPTPDDWord4.TimeStampHHigh) << 32);
        tv_nsec = le32_to_cpu(descv3->RxDescTimeStamp.TimeStampLow);

        skb_hwtstamps(skb)->hwtstamp = ktime_set(tv_sec, tv_nsec);
}
