/*
 * NRC7292 TXQ Fairness and QoS Improvements
 * 
 * This file contains improved implementations for:
 * 1. Fair credit distribution among TXQs
 * 2. AC priority-based processing
 * 3. Packet size-aware credit prediction
 *
 * Original code location: package/src/nrc/nrc-mac80211.c
 */

#include "nrc-mac80211.h"

/* AC priority order: Voice > Video > Best Effort > Background */
static const int ac_priority_order[] = {
    IEEE80211_AC_VO,  /* Voice */
    IEEE80211_AC_VI,  /* Video */
    IEEE80211_AC_BE,  /* Best Effort */
    IEEE80211_AC_BK   /* Background */
};

/**
 * count_active_txqs_by_ac - Count active TXQs for specific AC
 * @nw: NRC driver context
 * @target_ac: Target Access Category
 *
 * Returns the number of active TXQs for the specified AC
 */
static int count_active_txqs_by_ac(struct nrc *nw, int target_ac)
{
    struct nrc_txq *ntxq;
    int count = 0;

    list_for_each_entry(ntxq, &nw->txq, list) {
        if (ntxq->hw_queue == target_ac) {
            count++;
        }
    }

    return count;
}

/**
 * count_total_active_txqs - Count total active TXQs
 * @nw: NRC driver context
 *
 * Returns the total number of active TXQs across all ACs
 */
static int count_total_active_txqs(struct nrc *nw)
{
    struct nrc_txq *ntxq;
    int count = 0;

    list_for_each_entry(ntxq, &nw->txq, list) {
        count++;
    }

    return count;
}

/**
 * get_packet_credit_requirement - Calculate credit requirement for packet
 * @nw: NRC driver context
 * @skb: Socket buffer containing the packet
 *
 * Returns the number of credits required for this packet
 */
static int get_packet_credit_requirement(struct nrc *nw, struct sk_buff *skb)
{
    return DIV_ROUND_UP(skb->len, nw->fwinfo.buffer_size);
}

/**
 * nrc_push_txq_improved - Improved TXQ processing with fair credit distribution
 * @nw: NRC driver context
 * @ntxq: Target TXQ to process
 * @max_credit: Maximum credits allowed for this TXQ
 *
 * Returns:
 * 0: All packets processed or TXQ empty
 * 1: Credit limit reached, more packets remain
 */
static int nrc_push_txq_improved(struct nrc *nw, struct nrc_txq *ntxq, int max_credit)
{
    struct sk_buff *skb;
    struct ieee80211_txq *txq;
    struct ieee80211_tx_control control;
    int ac, available_credit;
    int used_credit = 0;
    int ret = 0;

    txq = to_txq(ntxq);
    ac = ntxq->hw_queue;
    available_credit = nrc_ac_credit(nw, ac);

    /* Use the minimum of available credit and allocated max credit */
    int credit_limit = min(available_credit, max_credit);

    if (credit_limit <= 0) {
        return 1; /* No credits available */
    }

    control.sta = ntxq->sta;

    rcu_read_lock();

    /* Process packets with credit-aware approach */
    while (used_credit < credit_limit) {
        /* Peek at next packet to check credit requirement */
        skb = ieee80211_tx_dequeue_peek(nw->hw, txq);
        if (!skb) {
            break; /* No more packets */
        }

        /* Calculate required credits for this packet */
        int required_credit = get_packet_credit_requirement(nw, skb);
        
        /* Check if we have enough credits */
        if (used_credit + required_credit > credit_limit) {
            ret = 1; /* Not enough credits, more packets remain */
            break;
        }

        /* Actually dequeue and transmit the packet */
        skb = ieee80211_tx_dequeue(nw->hw, txq);
        if (!skb) {
            break; /* Packet disappeared between peek and dequeue */
        }

        nrc_mac_tx(nw->hw, &control, skb);
        used_credit += required_credit;

        /* Update statistics */
        ntxq->nr_push_allowed++;
    }

    rcu_read_unlock();

    return ret;
}

/**
 * process_txqs_by_ac_priority - Process TXQs for specific AC with priority
 * @nw: NRC driver context
 * @target_ac: Target Access Category
 * @remaining_credit: Pointer to remaining credit counter
 *
 * Processes all TXQs belonging to the specified AC fairly
 */
static void process_txqs_by_ac_priority(struct nrc *nw, int target_ac, int *remaining_credit)
{
    struct nrc_txq *ntxq, *tmp;
    int ac_txq_count;
    int credit_per_txq;

    if (*remaining_credit <= 0) {
        return;
    }

    /* Count TXQs for this AC */
    ac_txq_count = count_active_txqs_by_ac(nw, target_ac);
    if (ac_txq_count == 0) {
        return;
    }

    /* Calculate fair credit distribution for this AC */
    credit_per_txq = max(1, *remaining_credit / ac_txq_count);

    /* Process TXQs belonging to this AC */
    list_for_each_entry_safe(ntxq, tmp, &nw->txq, list) {
        if (ntxq->hw_queue != target_ac) {
            continue; /* Skip TXQs from other ACs */
        }

        if (*remaining_credit <= 0) {
            break; /* No more credits available */
        }

        int allocated_credit = min(credit_per_txq, *remaining_credit);
        int ret = nrc_push_txq_improved(nw, ntxq, allocated_credit);

        if (ret == 0) {
            /* All packets processed, remove from active list */
            list_del_init(&ntxq->list);
        } else {
            /* More packets remain, move to end for round-robin */
            list_move_tail(&ntxq->list, &nw->txq);
        }

        /* Estimate used credits (simplified) */
        int estimated_used = min(allocated_credit, 
                               count_active_txqs_by_ac(nw, target_ac) > 0 ? allocated_credit : 0);
        *remaining_credit -= estimated_used;
    }
}

/**
 * nrc_tx_tasklet_improved - Improved TX tasklet with fairness and QoS
 * @t: Tasklet structure (for CONFIG_NEW_TASKLET_API)
 *
 * This improved version provides:
 * 1. Fair credit distribution among TXQs
 * 2. AC priority-based processing (VO > VI > BE > BK)
 * 3. Better handling of credit exhaustion
 */
#ifdef CONFIG_NEW_TASKLET_API
void nrc_tx_tasklet_improved(struct tasklet_struct *t)
{
    struct nrc *nw = from_tasklet(nw, t, tx_tasklet);
#else
void nrc_tx_tasklet_improved(unsigned long cookie)
{
    struct nrc *nw = (struct nrc *)cookie;
#endif
    int total_active_txqs;
    int total_available_credit = 0;
    int remaining_credit;
    int i;

    spin_lock_bh(&nw->txq_lock);

    /* Count total active TXQs */
    total_active_txqs = count_total_active_txqs(nw);
    if (total_active_txqs == 0) {
        goto unlock_and_exit;
    }

    /* Calculate total available credits across all ACs */
    for (i = 0; i < IEEE80211_NUM_ACS; i++) {
        total_available_credit += nrc_ac_credit(nw, i);
    }

    if (total_available_credit <= 0) {
        goto unlock_and_exit; /* No credits available */
    }

    remaining_credit = total_available_credit;

    /*
     * Process TXQs in AC priority order: Voice > Video > Best Effort > Background
     * This ensures QoS requirements are met for real-time traffic
     */
    for (i = 0; i < ARRAY_SIZE(ac_priority_order); i++) {
        process_txqs_by_ac_priority(nw, ac_priority_order[i], &remaining_credit);
        
        if (remaining_credit <= 0) {
            break; /* All credits consumed */
        }
    }

unlock_and_exit:
    spin_unlock_bh(&nw->txq_lock);
}

/**
 * nrc_wake_tx_queue_improved - Improved wake TX queue with better logging
 * @hw: IEEE 802.11 hardware
 * @txq: TX queue that has packets ready
 *
 * Enhanced version with debug information for better troubleshooting
 */
void nrc_wake_tx_queue_improved(struct ieee80211_hw *hw, struct ieee80211_txq *txq)
{
    struct nrc *nw = hw->priv;
    struct nrc_txq *ntxq = (void *)txq->drv_priv;
    unsigned long frame_cnt, byte_cnt;

    /* Power save handling - wake target if needed */
    if (nw->drv_state == NRC_DRV_PS) {
        nrc_hif_wake_target(nw->hif);
        nrc_mac_dbg("Waking target from power save for TXQ AC%d", ntxq->hw_queue);
    }

    /* Get queue depth for debugging */
    ieee80211_txq_get_depth(txq, &frame_cnt, &byte_cnt);

    spin_lock_bh(&nw->txq_lock);

    /* Add to active list only if not already present */
    if (list_empty(&ntxq->list)) {
        list_add_tail(&ntxq->list, &nw->txq);
        nrc_mac_dbg("Added TXQ AC%d to active list (frames:%lu, bytes:%lu)", 
                   ntxq->hw_queue, frame_cnt, byte_cnt);
    } else {
        nrc_mac_dbg("TXQ AC%d already in active list (frames:%lu, bytes:%lu)", 
                   ntxq->hw_queue, frame_cnt, byte_cnt);
    }

    spin_unlock_bh(&nw->txq_lock);

    /* Schedule TX tasklet for processing */
    nrc_kick_txq(nw);
}

/*
 * Statistics and debugging functions
 */

/**
 * nrc_txq_stats_show - Show TXQ statistics
 * @nw: NRC driver context
 *
 * Debugging function to show current TXQ state and statistics
 */
void nrc_txq_stats_show(struct nrc *nw)
{
    struct nrc_txq *ntxq;
    int ac_counts[IEEE80211_NUM_ACS] = {0};
    int total_active = 0;

    spin_lock_bh(&nw->txq_lock);

    list_for_each_entry(ntxq, &nw->txq, list) {
        ac_counts[ntxq->hw_queue]++;
        total_active++;
    }

    spin_unlock_bh(&nw->txq_lock);

    nrc_mac_dbg("TXQ Statistics:");
    nrc_mac_dbg("  Total active TXQs: %d", total_active);
    nrc_mac_dbg("  VO (AC3): %d, VI (AC2): %d, BE (AC1): %d, BK (AC0): %d",
               ac_counts[IEEE80211_AC_VO], ac_counts[IEEE80211_AC_VI],
               ac_counts[IEEE80211_AC_BE], ac_counts[IEEE80211_AC_BK]);

    for (int ac = 0; ac < IEEE80211_NUM_ACS; ac++) {
        int credit = nrc_ac_credit(nw, ac);
        nrc_mac_dbg("  AC%d available credits: %d", ac, credit);
    }
}