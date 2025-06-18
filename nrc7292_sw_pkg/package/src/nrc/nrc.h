/*
 * NRC7292 HaLow Driver Header File
 * 
 * This is a placeholder header file for the NRC7292 HaLow driver.
 * The actual implementation would contain detailed structure definitions,
 * function prototypes, and macro definitions for the driver.
 */

#ifndef _NRC_H_
#define _NRC_H_

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <net/mac80211.h>

/* Driver version information */
#define NRC_VERSION_MAJOR 1
#define NRC_VERSION_MINOR 0
#define NRC_VERSION_PATCH 0

/* Access Category definitions */
#define IEEE80211_NUM_ACS 4
#define NRC_MAX_TID 8

/* Main driver structure */
struct nrc {
    struct ieee80211_hw *hw;
    struct device *dev;
    
    /* Credit management */
    atomic_t tx_credit[IEEE80211_NUM_ACS*3];
    atomic_t tx_pend[IEEE80211_NUM_ACS*3];
    
    /* TX queue management */
    struct list_head txq;
    spinlock_t txq_lock;
    struct tasklet_struct tx_tasklet;
    
    /* Driver state */
    enum nrc_drv_state drv_state;
    
    /* Power management */
    bool ps_enabled;
    
    /* Firmware information */
    struct nrc_fw_info fwinfo;
};

/* STA structure for driver private data */
struct nrc_sta {
    struct nrc *nw;
    struct ieee80211_vif *vif;
    
    enum ieee80211_sta_state state;
    struct list_head list;
    
    /* Block ACK session management */
    enum ieee80211_tx_ba_state tx_ba_session[NRC_MAX_TID];
    uint32_t ba_req_last_jiffies[NRC_MAX_TID];
};

/* Convert ieee80211_sta to nrc_sta */
#define to_i_sta(s) ((struct nrc_sta *) (s)->drv_priv)

/* Function prototypes */
int nrc_init(void);
void nrc_exit(void);
int nrc_mac_tx(struct ieee80211_hw *hw, struct ieee80211_tx_control *control,
               struct sk_buff *skb);

#endif /* _NRC_H_ */