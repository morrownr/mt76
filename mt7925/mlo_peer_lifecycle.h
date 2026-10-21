/* SPDX-License-Identifier: ISC */
#ifndef __MT7925_MLO_PEER_LIFECYCLE_H
#define __MT7925_MLO_PEER_LIFECYCLE_H

enum mt7925_mlo_peer_state {
	MT7925_MLO_PEER_STATE_NONE,
	MT7925_MLO_PEER_STATE_ASSOC = 2,
};

struct mt7925_mlo_peer_lifecycle {
	unsigned int generation;
	unsigned int from_links;
	unsigned int to_links;
	unsigned int create_state;
	unsigned int authorize_state;
	unsigned int create_newly;
	unsigned int authorize_newly;
	int restored;
	int authorized;
	int converged;
	int ready;
};

static inline void
mt7925_mlo_peer_lifecycle_begin(struct mt7925_mlo_peer_lifecycle *lifecycle,
				unsigned int generation,
				unsigned int from_links,
				unsigned int to_links)
{
	*lifecycle = (struct mt7925_mlo_peer_lifecycle) {
		.generation = generation,
		.from_links = from_links,
		.to_links = to_links,
		.create_state = MT7925_MLO_PEER_STATE_NONE,
		.authorize_state = MT7925_MLO_PEER_STATE_ASSOC,
		.create_newly = 1,
		.authorize_newly = 0,
	};
}

static inline void
mt7925_mlo_peer_lifecycle_created(struct mt7925_mlo_peer_lifecycle *lifecycle)
{
	lifecycle->ready = 0;
}

static inline void
mt7925_mlo_peer_lifecycle_restored(struct mt7925_mlo_peer_lifecycle *lifecycle)
{
	lifecycle->restored = 1;
}

static inline void
mt7925_mlo_peer_lifecycle_authorized(struct mt7925_mlo_peer_lifecycle *lifecycle,
				     int ret)
{
	lifecycle->authorized = lifecycle->restored && !ret;
	lifecycle->converged = lifecycle->authorized;
	lifecycle->ready = lifecycle->authorized;
}

static inline int
mt7925_mlo_peer_lifecycle_terminal_authorized(const struct mt7925_mlo_peer_lifecycle *lifecycle)
{
	return lifecycle->authorized &&
	       lifecycle->authorize_state == MT7925_MLO_PEER_STATE_ASSOC &&
	       !lifecycle->authorize_newly;
}

static inline void
mt7925_mlo_peer_lifecycle_ordinary_update(struct mt7925_mlo_peer_lifecycle *lifecycle)
{
	*lifecycle = (struct mt7925_mlo_peer_lifecycle) {
		.authorize_state = MT7925_MLO_PEER_STATE_ASSOC,
		.authorize_newly = 0,
		.authorized = 1,
		.converged = 1,
		.ready = 1,
	};
}

#endif
