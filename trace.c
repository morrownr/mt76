// SPDX-License-Identifier: BSD-3-Clause-Clear
/*
 * Copyright (C) 2016 Felix Fietkau <nbd@nbd.name>
 */

#include <linux/module.h>

#ifndef __CHECKER__
#define CREATE_TRACE_POINTS
#include "trace.h"

EXPORT_TRACEPOINT_SYMBOL_GPL(mac_txdone);
EXPORT_TRACEPOINT_SYMBOL_GPL(dev_irq);
EXPORT_TRACEPOINT_SYMBOL_GPL(mlo_tx_select);
EXPORT_TRACEPOINT_SYMBOL_GPL(mlo_txwi);
EXPORT_TRACEPOINT_SYMBOL_GPL(mlo_txfree);
EXPORT_TRACEPOINT_SYMBOL_GPL(mlo_txs);
EXPORT_TRACEPOINT_SYMBOL_GPL(mlo_rx);

#endif
